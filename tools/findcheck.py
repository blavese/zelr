"""Ctrl+F, and the bar that is not a start menu.

The dock used to carry a wide field down the middle of it whose only job
was to open the launcher -- a second way to start a program, on a desktop
that already had one, sitting in the part of the bar the window chips grow
into. It is a button in the tray now, and what it opens looks through what
is on the screen.

Which is the part worth checking, because it is the part that cannot be
faked: a window is a rectangle of pixels and there is no text in a picture
of text, so the desktop can only find a word if the program showing it said
so. These checks are therefore about a word that is on the screen and a word
that is not, in a page nothing here wrote.

  python tools/findcheck.py [--keep]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402
from webserver import Server                             # noqa: E402

DISK = os.path.join(ROOT, "findcheck.%d.img" % os.getpid())

SCREEN_W, SCREEN_H = 1024, 768
DOCK_H, DOCK_GAP = 44, 14
DOCK_Y = SCREEN_H - DOCK_H - DOCK_GAP

# The band the dock occupies, and the band just above it where the find bar
# opens. Compared rather than read: what matters is that something appeared
# in one and nothing remains in the other.
DOCK = (0, DOCK_Y, SCREEN_W, DOCK_Y + DOCK_H)
ABOVE = (SCREEN_W // 2, DOCK_Y - 52, SCREEN_W, DOCK_Y - 4)
MIDDLE = (SCREEN_W // 2 - 180, DOCK_Y + 6, SCREEN_W // 2 + 180,
          DOCK_Y + DOCK_H - 6)


def region(px, w, rect):
    x0, y0, x1, y1 = rect
    out = bytearray()
    for y in range(y0, y1):
        out += px[(y * w + x0) * 3:(y * w + x1) * 3]
    return bytes(out)


def spread(b):
    """How many different colours are in a band, which is how much is drawn
    in it: an empty stretch of dock is two or three, and anything with a
    box and words on it is dozens."""
    return len(set(b[i:i + 3] for i in range(0, len(b), 3)))


# What the browser paints behind a word it was asked to find. Nothing else
# on the screen is this colour, so counting it is the same question as
# "was the word found, and shown where it is".
LIT = bytes((0xFF, 0xE5, 0x8F))


def lit_pixels(px, w, rect):
    b = region(px, w, rect)
    return sum(1 for i in range(0, len(b), 3) if b[i:i + 3] == LIT)


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("find")

    with Server() as srv:
        vm = Guest(DISK, memory=256, extra=srv.qemu_args())
        try:
            vm.wait_boot()
            vm.run("dhcp", timeout=25)
            vm.type("desktop\n")
            time.sleep(7)
            mon = vm.monitor()

            vm.type("browser http://%s/\n" % srv.host)
            time.sleep(14)
            mon.move_to(20, 20)
            time.sleep(1.5)

            w, h, px, shot = mon.screen("fn-before")
            middle_quiet = spread(region(px, w, MIDDLE))
            c.add("the middle of the dock is bare", middle_quiet <= 6, shot)
            if middle_quiet > 6:
                print("      %d colours where the bar used to be" % middle_quiet)

            before_above = spread(region(px, w, ABOVE))
            PAGE = (200, 120, 900, 600)
            page_before = region(px, w, PAGE)

            # --- ctrl+f -------------------------------------------------
            vm.type("\x06")
            time.sleep(2)
            w, h, px, shot = mon.screen("fn-open")
            c.add("ctrl+f opens something above the dock",
                  spread(region(px, w, ABOVE)) > before_above + 4, shot)

            # --- a word that is on the page -----------------------------
            #
            # "connection" is in the test server's page and nowhere on the
            # desktop, so finding it is this reading the page rather than
            # anything the desktop already knew. The browser paints behind
            # what it found and nothing else on screen is that colour, so
            # counting those pixels asks the question exactly.
            vm.type("connection", gap=0.18)
            time.sleep(3)
            w, h, px, shot = mon.screen("fn-hit")
            hit_lit = lit_pixels(px, w, PAGE)
            c.add("a word that is on the screen is found and shown",
                  hit_lit > 60, shot)
            if hit_lit <= 60:
                print("      %d lit pixels for a word that is there" % hit_lit)
            c.add("and the count above the dock says so",
                  spread(region(px, w, ABOVE)) > 8, shot)
            c.add("and the page it is on changed to show it",
                  region(px, w, PAGE) != page_before, shot)

            # --- and one that is not ------------------------------------
            for _ in range(len("connection")):
                vm.type(chr(8), gap=0.12)
            time.sleep(2)
            vm.type("zzzqqq", gap=0.18)
            time.sleep(3)
            w, h, px, shot = mon.screen("fn-miss")
            c.add("a word that is not there lights nothing",
                  lit_pixels(px, w, PAGE) == 0, shot)
            c.add("and is still answered rather than ignored",
                  spread(region(px, w, ABOVE)) > 8, shot)

            # --- escape ---------------------------------------------------
            vm.type("\x1b")
            time.sleep(2)
            w, h, px, shot = mon.screen("fn-closed")
            c.add("escape puts it away",
                  spread(region(px, w, ABOVE)) <= before_above + 4, shot)
        finally:
            vm.stop()
            try:
                os.remove(DISK)
            except OSError:
                pass

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
