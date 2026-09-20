"""Takes the pictures the readme uses.

Screenshots in a readme go stale silently. The one that was there had been
taken before the desktop had a design, a taskbar that floats, a typeface with
weights, or any of the windows it now shows, and nothing anywhere would ever
have said so.

So they are made the same way the tests are: boot the thing, drive it, and
photograph what it actually does. Run this after any change to how the desktop
looks and commit what comes out.

  python tools/shots.py

The PNG writer is here because the pictures come out of QEMU as P6 PPM and
nothing in this project is allowed to depend on an image library. zlib is in
the Python standard library and does the only hard part.
"""
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import (Guest, build_once, count_in, count_near,
                     ROOT)                                # noqa: E402
from webserver import Server                               # noqa: E402

DISK = os.path.join(ROOT, "shots.%d.img" % os.getpid())
OUT = os.path.join(ROOT, "docs")

SCREEN_W, SCREEN_H = 1024, 768

# Written the way wm.c writes it, rather than as the numbers that came out
# of it once. Every one of these moved when the panel went flush to the
# bottom edge and the menu's rows got shorter, and a picture taken with the
# old ones is a picture of the pointer landing somewhere else.
TASKBAR_H, TASKBAR_GAP = 44, 14
DOCK_SIDE = 16
PANEL_Y = SCREEN_H - TASKBAR_H - TASKBAR_GAP

BADGE_W = 76
CHIPS_X = DOCK_SIDE + 16 + BADGE_W + 18

# The launcher is two columns now: the kinds down the left, the things of
# that kind down the right, and as tall as the longer of them.
MENU_ITEM, MENU_PAD = 32, 10
MENU_RAIL, MENU_PANE = 132, 152
MENU_W = MENU_PAD * 2 + MENU_RAIL + MENU_PANE
MENU_ROWS = 5
MENU_H = MENU_PAD * 2 + MENU_ROWS * MENU_ITEM
MENU_LEFT = DOCK_SIDE
MENU_TOP = PANEL_Y - MENU_H - 8
MENU_RECT = (MENU_LEFT + 4, MENU_TOP + 4,
             MENU_LEFT + MENU_W - 4, MENU_TOP + MENU_H - 4)

# The surface everything on this desktop is built from, which is what says a
# menu or a window is covering the wallpaper. Laid down at an alpha just
# short of opaque, so a trace of what is behind comes through and an exact
# count finds fewer pixels than are there.
MENU_PANEL = (0xF4, 0xF4, 0xF7)
MENU_TOL = 6

LAUNCHER = (DOCK_SIDE + 16 + BADGE_W // 2, PANEL_Y + TASKBAR_H // 2)
PARK = (1010, SCREEN_H - 10)

# The dock holds a chip for each window, past the badge, with the terminal
# first. Its chip is what brings the terminal back.
TERMINAL_ICON = (CHIPS_X + 30, PANEL_Y + TASKBAR_H // 2)

# Somewhere the pointer is not asking for the panel. Anywhere along the
# bottom of the screen brings it back out over whatever is maximised, which
# is the opposite of what the picture is of.
HIGH = (1010, 300)

# Everything above the panel, which is where windows are.
ABOVE = (0, 0, SCREEN_W, PANEL_Y)

# The part of the panel that says what is running: the pinned icons, which
# get a bar under them while their program is up, and then the buttons for
# every window that is not one of those. Stopping short of the tray, because
# the clock in it changes on its own.
#
# This, rather than counting how much window is on the screen, is what says
# a program started. A window opening over another one of the same grey can
# leave that count unchanged or lower, which is how the calculator opening
# on top of the monitor read as the calculator not opening.
#
# The pins alone were not enough either: a machine starts with five of them
# and the monitor and the calculator are not among them, so they arrive as
# buttons further along and nothing under the icons moved at all. Both
# programs were running the whole time the check was saying they had not
# started. Compared as bytes, so it does not encode any colour.
PANEL_BAND = (CHIPS_X - 6, PANEL_Y + 4, 840, PANEL_Y + TASKBAR_H - 1)

# The terminal's own page, which is neither the wallpaper nor any window
# this desktop draws, so counting it is a check that a terminal is up.
SLATE = (0x10, 0x14, 0x1A)


def write_png(path, w, h, pixels):
    """A PPM's worth of pixels, as a PNG.

    Colour type 2 is 8 bit RGB with no palette and no alpha, which is what
    comes out of the framebuffer. Every row carries a filter byte of zero,
    meaning the row is stored as it is: filtering would compress better but
    it would also be the only clever thing in this file.
    """
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        raw += pixels[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")

    with open(path, "wb") as f:
        f.write(png)
    return len(png)


# What the launcher lists: which kind a thing is in, and where it is in
# that kind. One number was enough when the menu was one list of thirteen.
TERMINAL, FILES, NOTES, CALC = (0, 0), (0, 1), (0, 2), (0, 3)
BROWSER = (1, 0)
PAINT, MUSIC = (2, 0), (2, 1)
SETTINGS, MONITOR, ABOUT = (3, 0), (3, 1), (3, 2)
CLOSE_ALL = (4, 0)


def rail_row(i):
    """The middle of the ith kind, down the left."""
    return (MENU_LEFT + MENU_PAD + 40,
            MENU_TOP + MENU_PAD + i * MENU_ITEM + MENU_ITEM // 2)


def pane_row(j):
    """The middle of the jth thing of whichever kind is open."""
    return (MENU_LEFT + MENU_PAD + MENU_RAIL + 50,
            MENU_TOP + MENU_PAD + j * MENU_ITEM + MENU_ITEM // 2)


def open_launcher(mon):
    """The launcher, and proof that it opened.

    This used to click and sleep. A click that misses and a click that
    lands look exactly alike from here, so when one missed the entry click
    after it went to the wallpaper, nothing was launched, and what got
    written to docs/ was a photograph of an empty desktop with the right
    filename on it. Two of them were committed that way before anybody
    looked. So the menu is waited for, and a run that cannot open it stops
    rather than carrying on taking pictures of nothing."""
    _, _, _, _, ok = mon.click_for(
        LAUNCHER[0], LAUNCHER[1], "launcher-open",
        lambda w, h, px: count_near(px, w, MENU_RECT, MENU_PANEL,
                                    MENU_TOL) > 8000,
        timeout=25)
    if not ok:
        raise SystemExit("the launcher did not open")


def region(px, w, rect):
    x0, y0, x1, y1 = rect
    out = bytearray()
    for y in range(y0, y1):
        out += px[(y * w + x0) * 3:(y * w + x1) * 3]
    return bytes(out)


def panel_now(mon):
    """What the panel says is running, at this moment."""
    w, h, px, ppm = mon.screen("panel")
    try:
        os.remove(ppm)
    except OSError:
        pass
    return region(px, w, PANEL_BAND)


def started(mon, x, y, name, before, timeout=45):
    """Click, and wait for the panel to say another program is running."""
    _, _, _, _, ok = mon.click_for(
        x, y, name,
        lambda w, h, px: region(px, w, PANEL_BAND) != before, timeout=timeout)
    if not ok:
        raise SystemExit("nothing started from " + name)


def pick(mon, where):
    """The kind, then the thing. The pointer resting on a kind opens it, so
    the first half of this is a move rather than a click."""
    cat, item = where
    mon.move_to(*rail_row(cat))
    time.sleep(0.5)
    return pane_row(item)


def run_app(mon, index, name, tries=3):
    """Open the launcher, pick an entry, and make sure a program started.

    The whole gesture is repeated, not just the second half of it. Clicking
    the entry again after a click that did not take lands on the wallpaper,
    because the first one closed the menu whatever else it did: the retry
    could never work, and what it produced was three clicks on the desktop
    and a report that the program would not start."""
    for attempt in range(tries):
        before = panel_now(mon)
        open_launcher(mon)
        x, y = pick(mon, index)
        _, _, _, _, ok = mon.click_for(
            x, y, name,
            lambda w, h, px: region(px, w, PANEL_BAND) != before,
            timeout=30, tries=1)
        if ok:
            return
    raise SystemExit("nothing started from " + name)


def close_all(mon):
    """The other direction: everything gone, and the screen back to
    wallpaper."""
    open_launcher(mon)
    x, y = pick(mon, CLOSE_ALL)
    _, _, _, _, ok = mon.click_for(
        x, y, "close-all",
        lambda w, h, px: count_near(px, w, ABOVE, MENU_PANEL,
                                    MENU_TOL) < 4000,
        timeout=30)
    if not ok:
        raise SystemExit("close all left something on the screen")


def stroke(mon, x0, y0, steps):
    """A drag, for drawing in Paint.

    The button is held down across a run of relative moves rather than
    clicked at each point, because a press and a release inside one pass of
    the window manager's loop is a click it never sees at all.
    """
    mon.move_to(x0, y0)
    mon.send("mouse_button 1", settle=0.3)
    for dx, dy in steps:
        mon.send("mouse_move %d %d" % (dx, dy), settle=0.06)
    mon.send("mouse_button 0", settle=0.4)


def shoot(mon, name, settle=0.6, park=PARK):
    """One picture, parked so the pointer is never mid-screen in it."""
    mon.move_to(*park)
    time.sleep(settle)
    w, h, px, ppm = mon.screen("shot-" + name)

    path = os.path.join(OUT, name + ".png")
    size = write_png(path, w, h, px)
    try:
        os.remove(ppm)
    except OSError:
        pass
    print("  %-14s %dx%d  %d KiB" % (name + ".png", w, h, size // 1024))


def main():
    build_once()
    os.makedirs(OUT, exist_ok=True)

    # A web server on this machine, so the browser's picture is of a page
    # that was actually fetched rather than of one typed into a file. This
    # project's own server rather than a site on the internet, because a
    # picture that needs somebody else to be up is a picture that will one
    # day be of an error message.
    srv = Server()
    srv.__enter__()

    vm = Guest(DISK, memory=192, extra=srv.qemu_args())
    try:
        vm.wait_boot()
        vm.run("dhcp", timeout=25)
        mon = vm.monitor()

        # --- the console, before anything graphical happens ---------------
        vm.type("uname\n")
        time.sleep(1.0)
        vm.type("disk\n")
        time.sleep(1.0)
        shoot(mon, "boot")

        # --- the desktop, with the terminal it opens with -----------------
        vm.type("desktop\n")
        time.sleep(4.0)
        vm.type("help\n")
        time.sleep(1.5)
        shoot(mon, "desktop")

        # --- the launcher open over it ------------------------------------
        open_launcher(mon)
        shoot(mon, "launcher")

        # --- two windows, and which one has focus -------------------------
        #
        # The menu is already up from the picture above, so this picks an
        # entry out of it rather than opening it again.
        before = panel_now(mon)
        fx, fy = pick(mon, FILES)
        started(mon, fx, fy, "files", before)
        time.sleep(1.5)
        shoot(mon, "files")

        # --- paint, with something in it ----------------------------------
        run_app(mon, PAINT, "paint")
        stroke(mon, 300, 300, [(14, 10)] * 14)
        stroke(mon, 300, 440, [(14, -10)] * 14)
        stroke(mon, 520, 280, [(0, 12)] * 13)
        shoot(mon, "paint")

        # --- and settings, where the colours come from --------------------
        run_app(mon, SETTINGS, "settings")
        shoot(mon, "settings")

        # --- a wallpaper that moves, with nothing in front of it ----------
        #
        # The terminal is brought to the front first. Typing goes to whatever
        # window has focus, and what had it was the settings window, which
        # ignored the whole line: the picture came out showing the wallpaper
        # that was already on.
        mon.click(*TERMINAL_ICON)
        time.sleep(1.5)
        vm.type("write /zelr.cfg wallpaper 7\n")
        time.sleep(1.5)
        mon.send("sendkey alt-d", settle=1.8)
        shoot(mon, "wallpaper")


        # --- the monitor, on its own, once it has history to draw ---------
        #
        # Everything else is closed first. The other pictures are of windows
        # over windows on purpose, but this one is a graph, and a graph is
        # worth looking at only if nothing is sitting on it.
        #
        # It samples four times a second and keeps thirty seconds, so a
        # picture taken as it opens is an empty box. The wait is what puts
        # something in it.
        close_all(mon)
        run_app(mon, MONITOR, "monitor")
        time.sleep(10.0)
        shoot(mon, "monitor")

        # --- and the calculator over it, with an answer in it -------------
        #
        # Typed rather than clicked: the keys are a grid sized from the
        # window, and a remembered position for one of them is the bug this
        # project keeps finding.
        run_app(mon, CALC, "calculator")
        vm.type("78/4=")
        time.sleep(1.2)
        shoot(mon, "calc")

        # --- and the panel out of the way of a maximised window -----------
        #
        # Last, because a maximised window tucks the panel away, and the
        # launcher lives on the panel: every shot that needs the launcher
        # has to happen while there is still one to click.
        #
        # Nothing is running by now, so the terminal's icon on the panel
        # starts one rather than raising one, and this waits for it the same
        # way the launcher does.
        # Waited for on the terminal's own dark page rather than on the
        # panel, because the pointer ends up resting on the icon it just
        # clicked and that alone changes the panel whatever happened.
        _, _, _, _, ok = mon.click_for(
            TERMINAL_ICON[0], TERMINAL_ICON[1], "terminal-again",
            lambda w, h, px: count_in(px, w, ABOVE, SLATE) > 50000,
            timeout=45)
        if not ok:
            raise SystemExit("the terminal did not come back")
        time.sleep(1.5)

        # --- the browser, on a page off a real server ----------------------
        #
        # Before the terminal is maximised, because a maximised window fills
        # the screen and a picture of the browser with one behind it is a
        # picture of the wrong thing.
        vm.type("browser http://" + srv.host + "/\n")
        time.sleep(16)
        shoot(mon, "browser", park=HIGH)

        before = panel_now(mon)
        started(mon, TERMINAL_ICON[0], TERMINAL_ICON[1], "term-front", before)
        time.sleep(1.5)
        vm.type("help\n")            # a fresh one, so give it something to say
        time.sleep(1.5)
        mon.send("sendkey alt-up", settle=1.8)
        shoot(mon, "maximised", park=HIGH)

    finally:
        vm.stop()
        srv.__exit__(None, None, None)
        try:
            os.remove(DISK)
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
