"""The browser, driven the way a person drives one.

None of this reads text off the screen. What it does instead is compare one
render against another, which turns out to be a stronger question than "is
the right word there":

  the same page sent two different ways has to come out identical, so the
  chunked decoder is checked against the plain one rather than against an
  idea of what chunked decoding looks like

  going somewhere and coming back has to land on the same pixels, so the
  history, the second fetch and the whole layout are checked at once

  a link has to be findable by its colour and clickable where it is drawn,
  so the hit testing is checked against the drawing rather than against the
  layout that produced both

  python tools/browsercheck.py [--keep]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import (Guest, Checks, build_once, centre_of,      # noqa: E402
                     count_in, ROOT)
from webserver import Server                                    # noqa: E402

DISK = os.path.join(ROOT, "browsercheck.%d.img" % os.getpid())

SCREEN_W, SCREEN_H = 1024, 768

# Where the window lands, worked out the way winsrv.c works it out: past the
# desktop's icon column, one cascade step in because the terminal took the
# first, and pulled back on to the screen if it would not fit.
WM_BORDER, WM_TITLE_H = 4, 20
WM_TOP = WM_BORDER + WM_TITLE_H
ICON_LEFT, ICON_CELL_W = 14, 78
CASCADE_X = ICON_LEFT + ICON_CELL_W + 14

WIN_CW, WIN_CH = 860, 620
WIN_X = CASCADE_X + 48
WIN_Y = 36 + 38
if WIN_X > SCREEN_W - (WIN_CW + WM_BORDER * 2):
    WIN_X = SCREEN_W - (WIN_CW + WM_BORDER * 2)

INNER_X = WIN_X + WM_BORDER
INNER_Y = WIN_Y + WM_TOP

# And the pieces of it, the way browser.c lays them out.
UI_PAD, UI_GAP, UI_BTN_H, UI_ROW, UI_SCROLL_W = 8, 6, 30, 26, 10
TOOLBAR_H = UI_PAD + UI_BTN_H + UI_PAD

BACK = (INNER_X + UI_PAD + 17, INNER_Y + UI_PAD + UI_BTN_H // 2)
BAR_X = INNER_X + UI_PAD + 34 + 4 + 34 + UI_GAP + 62 + UI_GAP
BAR = (BAR_X + 200, INNER_Y + UI_PAD + UI_BTN_H // 2)

VIEW_X = INNER_X + 3
VIEW_Y = INNER_Y + TOOLBAR_H + 3
VIEW_W = WIN_CW - 6 - UI_SCROLL_W
VIEW_H = WIN_CH - (TOOLBAR_H + 3) - UI_ROW - 3
PAGE = (VIEW_X + 2, VIEW_Y + 2, VIEW_X + VIEW_W - 2, VIEW_Y + VIEW_H - 2)

# The accent, which is what a link is drawn in. The second preset is what a
# machine nobody has touched starts with.
LINK = (0x6E, 0x8A, 0xE8)
PAPER = (0xFF, 0xFF, 0xFF)


def region(px, w, rect):
    x0, y0, x1, y1 = rect
    out = bytearray()
    for y in range(y0, y1):
        out += px[(y * w + x0) * 3:(y * w + x1) * 3]
    return bytes(out)


def page_now(mon, name):
    w, h, px, ppm = mon.screen(name)
    return region(px, w, PAGE), px, w, ppm


def go(vm, mon, url, settle=9.0):
    """Type an address into the bar, the way a person would."""
    mon.click(*BAR)
    time.sleep(1.0)
    vm.type(url + "\n")
    time.sleep(settle)
    mon.move_to(SCREEN_W - 20, 300)         # the pointer off everything
    time.sleep(0.8)


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("the browser")

    with Server() as srv:
        vm = Guest(DISK, memory=256, extra=srv.qemu_args())
        try:
            vm.wait_boot()
            vm.run("dhcp", timeout=25)
            vm.type("desktop\n")
            time.sleep(6)
            mon = vm.monitor()

            vm.type("browser http://%s/\n" % srv.host)
            time.sleep(14)
            mon.move_to(SCREEN_W - 20, 300)
            time.sleep(1.0)

            first, px, w, shot = page_now(mon, "br-first")
            c.add("a page off a real server is drawn in the window",
                  len(set(first)) > 8, shot)
            c.add("and it is on paper rather than on the desktop",
                  count_in(px, w, PAGE, PAPER) > 200000, shot)

            # --- a link, found where it is drawn ---------------------------
            spot = centre_of(px, w, SCREEN_H, LINK, within=PAGE)
            c.add("the one link on it is drawn in the accent",
                  spot is not None, shot)

            if spot is None:
                return c.report(keep=keep)

            w2, h2, px2, shot2, moved = mon.click_for(
                spot[0], spot[1], "br-second",
                lambda w, h, px: region(px, w, PAGE) != first, timeout=30)
            c.add("clicking it goes to the page it points at", moved, shot2)
            mon.move_to(SCREEN_W - 20, 300)
            time.sleep(0.8)
            second, px2, w2, shot2 = page_now(mon, "br-second-still")
            c.add("which is a different page from the one before it",
                  second != first, shot2)

            # --- and back --------------------------------------------------
            w3, h3, px3, shot3, back = mon.click_for(
                BACK[0], BACK[1], "br-back",
                lambda w, h, px: region(px, w, PAGE) == first, timeout=30)
            c.add("going back lands on the first page again, pixel for pixel",
                  back, shot3)

            # --- the same page, framed two ways ----------------------------
            #
            # One with a length on it and one in chunks. If the chunked
            # decoder loses a byte or keeps a size line, the two renders are
            # not the same, and nothing else here would notice.
            go(vm, mon, "http://%s/measured" % srv.host)
            measured, _, _, shotm = page_now(mon, "br-measured")
            c.add("a page with every shape in it renders",
                  len(set(measured)) > 8, shotm)

            go(vm, mon, "http://%s/framed" % srv.host)
            framed, _, _, shotf = page_now(mon, "br-framed")
            c.add("and the same page sent in chunks renders identically",
                  framed == measured, shotf)

            # --- a redirect ------------------------------------------------
            go(vm, mon, "http://%s/redirect" % srv.host)
            moved_to, _, _, shotr = page_now(mon, "br-redirect")
            c.add("a redirect is followed to the page it points at",
                  moved_to == second, shotr)

            # --- and something that is not there ---------------------------
            go(vm, mon, "http://%s/nothing-here" % srv.host)
            missing, pxm, wm_, shotn = page_now(mon, "br-404")
            c.add("a page that is not there says so rather than showing the "
                  "last one", missing != moved_to, shotn)

            # --- and an address it cannot speak ----------------------------
            go(vm, mon, "https://%s/" % srv.host, settle=5.0)
            secure, _, _, shots = page_now(mon, "br-https")
            c.add("https says it cannot do that rather than failing quietly",
                  secure != missing, shots)
        finally:
            vm.stop()
            try:
                os.remove(DISK)
            except OSError:
                pass

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
