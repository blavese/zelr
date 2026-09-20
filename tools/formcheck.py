"""Filling a form in and sending it, driven the way a person drives one.

The browser could read a page and not be used on one. An <input> was not
drawn at all -- the layout had a name for a field and never made one -- so
there was no box to click, nothing to type into, and no path from a filled
in form to a request. A search box is the commonest thing on the web and
this could not work one.

What makes this check worth having is who does the checking. It would be
easy to ask the browser what it thinks it sent; that is the client marking
its own work, and it passes just as happily when the escaping is wrong, when
a box nobody ticked is sent anyway, or when a field with no name is included
because including everything was simpler. So the server records what
actually arrived, and the checks below are about that.

The controls are found on the screen by colour rather than by arithmetic
about where they ought to be. A page can style a field like anything else,
so the test page gives each one a colour of its own, and finding it is then
the same question as "was it drawn where the browser thinks it is" -- which
is the thing a click depends on.

  python tools/formcheck.py [--keep]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import (Guest, Checks, build_once, centre_of,      # noqa: E402
                     ROOT)
from webserver import Server                                    # noqa: E402

DISK = os.path.join(ROOT, "formcheck.%d.img" % os.getpid())

SCREEN_W, SCREEN_H = 1024, 768

# The same window geometry browsercheck works out, for the same reasons.
WM_BORDER, WM_TITLE_H = 1, 32
WM_TOP = WM_BORDER + WM_TITLE_H
ICON_LEFT, ICON_CELL_W = 18, 92
CASCADE_X = ICON_LEFT + ICON_CELL_W + 14
DOCK_H, DOCK_GAP = 44, 14
WORK_H = SCREEN_H - DOCK_H - DOCK_GAP
WIN_CW, WIN_CH = 860, 620
WIN_X = CASCADE_X + 48
WIN_Y = 36 + 38
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
UI_PAD, UI_GAP, UI_BTN_H, UI_ROW, UI_SCROLL_W = 8, 6, 30, 26, 10
TOOLBAR_H = UI_PAD + UI_BTN_H + UI_PAD

BAR_X = INNER_X + UI_PAD + 34 + 4 + 34 + UI_GAP + 62 + UI_GAP
BAR = (BAR_X + 200, INNER_Y + UI_PAD + UI_BTN_H // 2)

VIEW_X = INNER_X + 3
VIEW_Y = INNER_Y + TOOLBAR_H + 3
VIEW_W = WIN_CW - 6 - UI_SCROLL_W
VIEW_H = WIN_CH - (TOOLBAR_H + 3) - UI_ROW - 3
PAGE = (VIEW_X + 2, VIEW_Y + 2, VIEW_X + VIEW_W - 2, VIEW_Y + VIEW_H - 2)
PARK = (PAGE[0] - 60, PAGE[3] + 8)

# What each control on the test page is painted, so it can be found.
FIELD = (0x00, 0xA0, 0x00)
SPARE = (0x00, 0xC0, 0xC0)
CHECK = (0xC0, 0x00, 0xC0)
UNTICKED = (0x80, 0x80, 0x00)
BUTTON = (0xE0, 0x80, 0x00)
LINK = (0x6E, 0x8A, 0xE8)


def region(px, w, rect):
    x0, y0, x1, y1 = rect
    out = bytearray()
    for y in range(y0, y1):
        out += px[(y * w + x0) * 3:(y * w + x1) * 3]
    return bytes(out)


def page_now(mon, name):
    w, h, px, ppm = mon.screen(name)
    return region(px, w, PAGE), px, w, ppm


def go(vm, mon, url, was=None, name="fm-going", settle=10.0):
    """An address typed into the bar, slowly.

    Slower than the harness types by default, and the reason is worth
    writing down. Bytes written to the serial port while the guest has not
    drained the port are dropped by the host before the kernel ever sees
    them, so typing only works as fast as the guest reads. The browser's
    idle loop asks to sleep between frames, and a sleep from a ring 3
    program can return with no time passed at all -- the scheduler hands
    the processor straight back when nothing else is ready -- so the
    browser spins instead of yielding and the console is not read often
    enough. At the default gap this lost two thirds of an address.

    That is the kernel's bug and not this one, and it is written up rather
    than worked around in the browser. Here it is waited out.
    """
    mon.click(*BAR)
    time.sleep(1.0)
    vm.type(url + "\n", gap=0.18)
    if was is None:
        time.sleep(settle)
    else:
        mon.wait_screen(name,
                        lambda w, h, px: region(px, w, PAGE) != was,
                        timeout=settle + 30)
    mon.move_to(*PARK)
    time.sleep(0.8)


def pairs_of(text):
    """The query a server was sent, as a set of name=value."""
    return set(p for p in text.split("&") if p)


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("forms")

    with Server() as srv:
        vm = Guest(DISK, memory=256, extra=srv.qemu_args())
        try:
            vm.wait_boot()
            vm.run("dhcp", timeout=25)
            vm.type("desktop\n")
            time.sleep(6)
            mon = vm.monitor()

            vm.type("browser http://%s/form\n" % srv.host)
            time.sleep(14)
            mon.move_to(*PARK)
            time.sleep(1.0)

            before, px, w, shot = page_now(mon, "fm-form")

            # --- the controls exist, and are where they are drawn ----------
            spot = centre_of(px, w, SCREEN_H, FIELD, within=PAGE)
            c.add("a text field on a page is drawn as a field", spot is not None,
                  shot)
            button = centre_of(px, w, SCREEN_H, BUTTON, within=PAGE)
            c.add("and a submit button as a button", button is not None, shot)
            box = centre_of(px, w, SCREEN_H, CHECK, within=PAGE)
            c.add("and a checkbox as a box", box is not None, shot)
            c.add("a hidden field is not drawn at all",
                  centre_of(px, w, SCREEN_H, (0x00, 0x00, 0x01),
                            within=PAGE) is None, shot)

            if spot is None or button is None or box is None:
                return c.report(keep=keep)

            # --- typing into it --------------------------------------------
            mon.click(*spot)
            time.sleep(0.6)
            vm.type("hello world")
            time.sleep(1.2)
            mon.move_to(*PARK)
            time.sleep(0.6)
            typed, px2, w2, shot2 = page_now(mon, "fm-typed")
            c.add("what is typed into it appears in it", typed != before, shot2)

            # --- and sending it ---------------------------------------------
            mon.click(*box)
            time.sleep(0.6)
            srv.forget()
            mon.click(*button)

            got = None
            for _ in range(60):
                if srv.received():
                    got = srv.received()[0]
                    break
                time.sleep(0.5)

            c.add("pressing the button sends the form", got is not None)
            if got is None:
                return c.report(keep=keep)

            how, text = got
            sent = pairs_of(text)
            c.add("as a GET, because that is what the form asked for",
                  how == "get")
            c.add("carrying what was typed, escaped", "q=hello+world" in sent)
            c.add("and the box that was ticked", "deep=yes" in sent)
            c.add("and a hidden field nobody could see", "from=zelr" in sent)
            c.add("but not a box that was left alone",
                  not any(p.startswith("off=") for p in sent))
            c.add("and not a field with no name to send it under",
                  not any(p.startswith("spare=") or p == "=ignored"
                          for p in sent))

            # --- the other method --------------------------------------------
            #
            # A form that posts is the one that carries a body, and the body
            # is the whole difference: a client that built the query
            # correctly and then put it in the address would pass every
            # check above and send a password in a server's log file.
            # The server having the request is not the browser having drawn
            # the answer: waiting on the first and photographing for the
            # second catches the page that was there before.
            mon.wait_screen("fm-said-wait",
                            lambda w, h, px: region(px, w, PAGE) != typed,
                            timeout=30)
            mon.move_to(*PARK)
            time.sleep(1.2)
            said, px3, w3, shot3 = page_now(mon, "fm-said")

            # Reached by clicking rather than by typing an address: an
            # address goes in through the serial port a character at a time
            # and this guest drops what it cannot drain in time, which loses
            # most of a long one and is nothing to do with forms.
            # Two words of link text is about a hundred and twenty pixels
            # of the accent exactly, which is under what centre_of asks for
            # by default.
            link = centre_of(px3, w3, SCREEN_H, LINK, min_pixels=60,
                             within=PAGE)
            c.add("the page it landed on has a way onward", link is not None,
                  shot3)
            if link is None:
                return c.report(keep=keep)

            mon.click_for(link[0], link[1], "fm-posts",
                          lambda w, h, px: region(px, w, PAGE) != said,
                          timeout=30)
            mon.move_to(*PARK)
            time.sleep(1.2)

            _, px4, w4, shot4 = page_now(mon, "fm-post-form")
            spot = centre_of(px4, w4, SCREEN_H, FIELD, within=PAGE)
            button = centre_of(px4, w4, SCREEN_H, BUTTON, within=PAGE)
            c.add("a form that posts is drawn the same way",
                  spot is not None and button is not None, shot4)

            if spot is None or button is None:
                return c.report(keep=keep)

            mon.click(*spot)
            time.sleep(0.6)
            vm.type("over the wire")
            time.sleep(1.2)
            srv.forget()
            mon.click(*button)

            got = None
            for _ in range(60):
                if srv.received():
                    got = srv.received()[0]
                    break
                time.sleep(0.5)

            c.add("a form asking to post is sent", got is not None)
            if got is not None:
                how, text = got
                c.add("as a POST", how == "post")
                whole = "q=over+the+wire" in pairs_of(text)
                if not whole:
                    print("      the server was sent %r" % (text,))
                c.add("with what was typed in the body rather than the address",
                      whole)
        finally:
            vm.stop()
            try:
                os.remove(DISK)
            except OSError:
                pass

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
