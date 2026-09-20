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
WM_BORDER, WM_TITLE_H = 1, 32
WM_TOP = WM_BORDER + WM_TITLE_H
#
# The icon column is where wm_icons_right() says it is, and it moved: the
# cells went from 78 wide to 92 and their left inset from 14 to 18, which
# put every control in this window eighteen pixels right of where the clicks
# below were aimed. The window opened, drew, and did nothing when pressed,
# and what that reported was a browser that would not follow a link.
ICON_LEFT, ICON_CELL_W = 18, 92
CASCADE_X = ICON_LEFT + ICON_CELL_W + 14

# The dock, because the work area is what the vertical clamp is measured
# against.
DOCK_H, DOCK_GAP = 44, 14
WORK_H = SCREEN_H - DOCK_H - DOCK_GAP

WIN_CW, WIN_CH = 860, 620
WIN_X = CASCADE_X + 48
WIN_Y = 36 + 38

# Pulled back on to the screen, both ways, the way winsrv.c does it. The
# horizontal clamp was here and the vertical one was not, which was fine
# while it never fired: a twenty pixel title bar and a work area reaching to
# thirty four from the bottom left a six hundred and twenty pixel window at
# seventy four still fitting. A thirty two pixel title bar and a dock that
# floats clear of the edge take thirty two pixels out of that between them,
# so the window is lifted eighteen pixels and everything in its toolbar went
# with it.
if WIN_X > SCREEN_W - (WIN_CW + WM_BORDER * 2):
    WIN_X = SCREEN_W - (WIN_CW + WM_BORDER * 2)
if WIN_Y > WORK_H - (WIN_CH + WM_TOP + WM_BORDER):
    WIN_Y = WORK_H - (WIN_CH + WM_TOP + WM_BORDER)
if WIN_X < 0:
    WIN_X = 0
if WIN_Y < 0:
    WIN_Y = 0

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

# Where the pointer goes to be out of the way.
#
# Two checks here compare one rendering of a page against another and want
# them identical, and the pointer is drawn by the window manager like
# everything else: a picture with it inside the rectangle being compared
# cannot match one without it.
#
# It used to park at the right hand edge of the screen, a few pixels clear
# of the page. Then the icon column grew, the window cascaded further right,
# and those few pixels became none: the pointer sat in the page, and "going
# back" compared a page with a pointer on it against the same page without
# one. What it reported was a browser that would not go back. It had gone
# back.
#
# Left of the window and below the page, which is wallpaper, and worked out
# from PAGE rather than from the screen so it cannot drift back in.
PARK = (PAGE[0] - 60, PAGE[3] + 8)
assert 0 < PARK[0] < PAGE[0], "the pointer is parked inside the page"

# The accent, which is what a link is drawn in. The second preset is what a
# machine nobody has touched starts with.
LINK = (0x6E, 0x8A, 0xE8)
PAPER = (0xFF, 0xFF, 0xFF)

# The colour of the picture the server serves, and of the drawing. Nothing
# else on the screen is this, so counting it asks whether the thing arrived,
# decoded and reached the glass -- three separate things, one number.
LOGO = (0xE1, 0x1D, 0x48)

# The colour /styled asks for its band to be. Nothing else on the screen is
# this colour, so counting it is the same question as "was the sheet read".
BAND = (0x1D, 0x4E, 0xD8)


def region(px, w, rect):
    x0, y0, x1, y1 = rect
    out = bytearray()
    for y in range(y0, y1):
        out += px[(y * w + x0) * 3:(y * w + x1) * 3]
    return bytes(out)


def page_now(mon, name):
    w, h, px, ppm = mon.screen(name)
    return region(px, w, PAGE), px, w, ppm


def page_settled(mon, name, steady=3, gap=0.4, tries=40):
    """The page once it has stopped changing, and stayed stopped.

    Two checks here compare one rendering against another and want them to
    be identical, which means the picture has to be of a page that finished
    drawing. A fixed sleep cannot promise that: a page delivered in chunks
    takes as long as it takes, and a picture taken in the middle of it
    differs from the same page delivered whole -- which reads as the chunked
    decoder being wrong when what happened was that nobody waited.

    Two identical frames four tenths of a second apart is not that either.
    It is "nothing changed in four hundred milliseconds", which happens in
    the middle of a draw often enough that the chunked check failed about
    one run in three. Measured on a run where both pages did finish, the two
    renders came out identical to the byte, so the decoder was never what
    was wrong.

    Three frames, spanning about a second, is a pause a draw does not
    survive. It costs one more screenshot on a page that had already
    stopped.
    """
    same = 0
    last = None
    got = None
    for _ in range(tries):
        now, px, w, ppm = page_now(mon, name)
        got = (now, px, w, ppm)
        if last is not None and now == last:
            same += 1
            if same >= steady - 1:
                return got
        else:
            same = 0
        last = now
        time.sleep(gap)
    return got if got else page_now(mon, name)


def go(vm, mon, url, settle=9.0, was=None, name="br-going"):
    """Type an address into the bar, the way a person would, and wait for the
    page to change.

    Waiting for the page rather than sleeping at it. This used to sleep a
    fixed nine seconds and then take its picture, which is too long for a
    page that arrived at once and not long enough for one that did not: on a
    host running a dozen machines, five checks in a row reported that the
    browser had not navigated when what had happened was that nobody waited.

    `was` is the page as it looked before, so there is something to compare
    against. Without it this falls back to the old sleep, which is right for
    the first page — there is nothing on screen yet for it to differ from.
    """
    mon.click(*BAR)
    time.sleep(1.0)
    vm.type(url + "\n")

    if was is None:
        time.sleep(settle)
    else:
        # A generous ceiling, because reaching it is a real failure and not
        # a slow machine. One connection at a time is all this kernel's TCP
        # does, so an address asked for straight after a redirect waits for
        # the last one to finish with the socket before it can start.
        mon.wait_screen(name,
                        lambda w, h, px: region(px, w, PAGE) != was,
                        timeout=settle + 30)

    mon.move_to(*PARK)                      # the pointer off the page
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
            mon.move_to(*PARK)
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
            mon.move_to(*PARK)
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
            # They are meant to come out identical, which is what makes
            # them awkward to arrive at: a page that renders the same as the
            # one before it cannot be told from never having left. Each is
            # reached from a page that looks like neither, so the journey
            # visibly changes something and the settle below has a finished
            # page to settle on.
            go(vm, mon, "http://%s/bare" % srv.host, settle=6.0)
            go(vm, mon, "http://%s/measured" % srv.host, settle=8.0)
            measured, _, _, shotm = page_settled(mon, "br-measured")
            c.add("a page with every shape in it renders",
                  len(set(measured)) > 8, shotm)

            go(vm, mon, "http://%s/bare" % srv.host, settle=6.0)
            go(vm, mon, "http://%s/framed" % srv.host, settle=8.0)
            framed, _, _, shotf = page_settled(mon, "br-framed")
            c.add("and the same page sent in chunks renders identically",
                  framed == measured, shotf)

            # --- style sheets ----------------------------------------------
            #
            # The strong form of this question is not "did the page render",
            # which it does with no sheets at all. It is whether the same
            # markup looks different with its sheets and without them, which
            # fails both when the sheets are ignored and when they are
            # applied to the wrong elements.
            go(vm, mon, "http://%s/styled" % srv.host, settle=12.0,
               was=framed, name="br-going-styled")
            styled, pxs, ws, shots = page_now(mon, "br-styled")
            c.add("a page with style sheets renders",
                  len(set(styled)) > 8, shots)
            c.add("and the band the sheet asks for is painted across it",
                  count_in(pxs, ws, PAGE, BAND) > 3000, shots)

            go(vm, mon, "http://%s/bare" % srv.host, settle=10.0,
               was=styled, name="br-going-bare")
            bare, pxb, wb, shotb = page_now(mon, "br-bare")
            c.add("the same markup with no sheets does not look the same",
                  bare != styled, shotb)
            c.add("and has no band on it at all",
                  count_in(pxb, wb, PAGE, BAND) < 200, shotb)

            # --- a picture -------------------------------------------------
            #
            # The whole way through: fetched over http as a second request,
            # inflated, unfiltered, turned into pixels and drawn at the size
            # the layout left for it.
            go(vm, mon, "http://%s/picture" % srv.host, settle=12.0,
               was=bare, name="br-going-picture")
            withpic, pxp, wp, shotp = page_settled(mon, "br-picture")
            c.add("a picture on a page is fetched, decoded and drawn",
                  count_in(pxp, wp, PAGE, LOGO) > 10000, shotp)

            # And one that is not there falls back to the words it carries,
            # which is what alt text is for. A browser that drew nothing at
            # all would look the same as one that drew the picture wrongly,
            # so this asks that the page differs from the one with it.
            go(vm, mon, "http://%s/missing-picture" % srv.host, settle=10.0,
               was=withpic, name="br-going-nopic")
            nopic, pxn, wn, shotn = page_settled(mon, "br-missing-picture")
            c.add("and one that is not there does not draw anything",
                  count_in(pxn, wn, PAGE, LOGO) < 100, shotn)
            c.add("so the two pages do not look the same",
                  nopic != withpic, shotn)

            # --- a drawing -------------------------------------------------
            #
            # An SVG is not a picture, it is instructions for making one, so
            # this asks a different question: not whether the bytes decoded
            # but whether the shapes were drawn, at the size the page asked
            # for rather than the size they describe. It catches a viewBox
            # read under the wrong name, which draws at the ratio of the
            # height asked for to a square the drawing never had.
            go(vm, mon, "http://%s/drawn" % srv.host, settle=12.0,
               was=nopic, name="br-going-drawn")
            drawn, pxd, wd, shotd = page_settled(mon, "br-drawn")
            c.add("a drawing on a page is rendered and drawn",
                  count_in(pxd, wd, PAGE, LOGO) > 10000, shotd)

            # --- a page that runs its own script ---------------------------
            #
            # The band is the whole check, and it asks a lot at once: nothing
            # in the markup has the class that paints it, so for it to be on
            # the screen the engine had to run, getElementById had to find the
            # element in the real document, the assignment had to reach that
            # document rather than a copy of it, and the cascade had to match
            # a class that was not there when the sheet was indexed.
            go(vm, mon, "http://%s/scripted" % srv.host, settle=12.0,
               was=drawn, name="br-going-scripted")
            scripted, pxc, wc, shotc = page_settled(mon, "br-scripted")
            c.add("a page's own script runs, and changes the page",
                  count_in(pxc, wc, PAGE, BAND) > 3000, shotc)

            go(vm, mon, "http://%s/unscripted" % srv.host, settle=10.0,
               was=scripted, name="br-going-unscripted")
            unscripted, pxu, wu, shotu = page_settled(mon, "br-unscripted")
            c.add("and the same page without the script does not",
                  count_in(pxu, wu, PAGE, BAND) < 200, shotu)
            c.add("so the two do not look the same",
                  unscripted != scripted, shotu)

            # --- a redirect ------------------------------------------------
            go(vm, mon, "http://%s/redirect" % srv.host,
               was=unscripted, name="br-going-redirect")
            moved_to, _, _, shotr = page_now(mon, "br-redirect")
            c.add("a redirect is followed to the page it points at",
                  moved_to == second, shotr)

            # --- and something that is not there ---------------------------
            go(vm, mon, "http://%s/nothing-here" % srv.host,
               was=moved_to, name="br-going-404")
            missing, pxm, wm_, shotn = page_now(mon, "br-404")
            c.add("a page that is not there says so rather than showing the "
                  "last one", missing != moved_to, shotn)

            # --- and an address it cannot speak ----------------------------
            go(vm, mon, "https://%s/" % srv.host, settle=5.0,
               was=missing, name="br-going-https")
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
