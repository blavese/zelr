"""A page that does its work after it has been read.

The readme said this browser could not: no event loop, no timer, no
addEventListener, and no fetching from a script, so a page that does its work
on a click did nothing at all. Three of those four had quietly become untrue
and nothing here was checking any of them, which is how a readme comes to
describe a program that stopped existing.

So this checks the four separately, and it checks them by colour. The test
page gives each band a colour of its own and every one of them starts blue;
a script turns one green on a click, a timer turns one orange, a file the
page links to turns one violet, and an answer from the network turns one
teal. None of those colours is anywhere else on the screen, so counting them
asks exactly the question -- did that happen -- and cannot be answered by a
browser that drew the page and ran none of it.

The same page with every script stripped out is served alongside, because
four counts of coloured pixels otherwise prove only that a page was drawn.
Against that one everything stays blue.

The request is the interesting one. Its handler is set *after* send(), which
only works if the request is really made later: a send() that fetched and
called back before it returned would find no handler there at all. A browser
that did it the easy way fails that check and passes every other one here.

  python tools/livecheck.py [--keep]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from webserver import Server                                    # noqa: E402

DISK = os.path.join(ROOT, "livecheck.%d.img" % os.getpid())

SCREEN_W = 1024
PAGE = (200, 120, 900, 620)

# One colour per thing that has to happen. Blue is where they all start.
BLUE   = (0x1D, 0x4E, 0xD8)
GREEN  = (0x00, 0xB0, 0x50)      # a click reached a listener
ORANGE = (0xFF, 0x80, 0x00)      # a timer went off
VIOLET = (0x80, 0x00, 0xFF)      # a script in a file of its own ran
TEAL   = (0x00, 0xA0, 0xA0)      # an answer came back from the network

# A band is about 43,000 pixels. Well under one, well over nothing.
ENOUGH = 8000


def seen(px, w, colour):
    return count_in(px, w, PAGE, colour)


def find_blue_band(px, w):
    """Where the first blue band is, found rather than remembered: the page
    is laid out by the browser and a coordinate written down here is a
    coordinate that goes stale the next time the font changes."""
    for y in range(PAGE[1], PAGE[3]):
        run = 0
        for x in range(PAGE[0], PAGE[2]):
            i = (y * w + x) * 3
            run = run + 1 if px[i:i + 3] == bytes(BLUE) else 0
            if run > 120:
                return (x - 60, y)
    return None


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("a page that does things")

    with Server() as srv:
        vm = Guest(DISK, memory=256, extra=srv.qemu_args())
        try:
            vm.wait_boot()
            vm.run("dhcp", timeout=25)
            vm.type("desktop\n")
            time.sleep(7)
            mon = vm.monitor()

            # --- the control, with every script taken out ----------------
            vm.type("browser http://%s/live-quiet\n" % srv.host)
            time.sleep(14)
            mon.move_to(20, 20)
            time.sleep(1.5)
            w, h, px, shot = mon.screen("live-quiet")
            c.add("a page with its scripts taken out is drawn",
                  seen(px, w, BLUE) > ENOUGH, shot)
            quiet = {n: seen(px, w, col) for n, col in
                     (("green", GREEN), ("orange", ORANGE),
                      ("violet", VIOLET), ("teal", TEAL))}
            c.add("and nothing on it has happened", all(v < 200 for v in quiet.values()),
                  shot)
            if any(v >= 200 for v in quiet.values()):
                print("      %r" % quiet)

            # --- and the same page with them in --------------------------
            #
            # A second browser rather than the address bar, because what
            # is being compared is two pictures of a whole window, and a
            # window in front of another one is not that. The first is
            # closed the way a person closes it.
            mon.send("sendkey alt-q", settle=1.5)
            time.sleep(2)
            vm.type("browser http://%s/live" % srv.host + chr(10))
            time.sleep(16)
            mon.move_to(20, 20)
            time.sleep(2)

            w, h, px, shot = mon.screen("live-open")
            c.add("a timer the page set goes off",
                  seen(px, w, ORANGE) > ENOUGH, shot)
            if seen(px, w, ORANGE) <= ENOUGH:
                print("      %d orange pixels" % seen(px, w, ORANGE))

            c.add("a script in a file of its own is fetched and run",
                  seen(px, w, VIOLET) > ENOUGH, shot)
            if seen(px, w, VIOLET) <= ENOUGH:
                print("      %d violet pixels" % seen(px, w, VIOLET))

            c.add("and an answer the page asked the network for comes back",
                  seen(px, w, TEAL) > ENOUGH, shot)
            if seen(px, w, TEAL) <= ENOUGH:
                print("      %d teal pixels" % seen(px, w, TEAL))

            # --- the click ------------------------------------------------
            c.add("nothing has been clicked yet", seen(px, w, GREEN) < 200, shot)

            at = find_blue_band(px, w)
            c.add("there is something on the page to press", at is not None)
            if at:
                w, h, px, shot, lit = mon.click_for(
                    at[0], at[1], "live-click",
                    lambda w, h, px: seen(px, w, GREEN) > ENOUGH)
                c.add("and pressing it reaches the page's own listener",
                      lit, shot)
                if not lit:
                    print("      %d green pixels after the click"
                          % seen(px, w, GREEN))
        finally:
            vm.stop()
            try:
                os.remove(DISK)
            except OSError:
                pass

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
