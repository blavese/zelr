"""YouTube, Twitch and a Google search, against the real sites.

userland/sites.h reads YouTube's pages from the data inside them and asks
Twitch's public API what its pages would show, and writes plain pages of its
own from that. sitetest checks the reader against pages of the shapes those
sites send, with no network. What it cannot check is whether the sites still
send those shapes: they change when they like, and a reader that has fallen
behind reads nothing. So this asks the sites themselves, the way tlscheck
asks real servers for their certificates, and fails without a connection for
the same reason -- a machine that cannot reach YouTube should not report that
it can read it.

Two halves. sitetest's live mode fetches and reads a page and says on the
console how many rows it made and how many pictures they point at, so the
numbers are the reader's and not a screenshot's. Then the browser, on the
desktop, is sent to each and says on the console what the page turned into,
which is what somebody sitting at it would see.

A Google search is not read: its results are made by a program that decides
whether a person is asking, and that is not something to get round. It is
answered by DuckDuckGo's page, which says so; this checks that it is.

  python tools/sitecheck.py [--keep]
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT              # noqa: E402
from browsercheck import BAR, PARK, PAGE                         # noqa: E402
from harness import count_in                                     # noqa: E402

DISK = os.path.join(ROOT, "sitecheck.%d.img" % os.getpid())

# What each page has to come to. The counts are floors well under what the
# sites give (twenty or so videos, twenty four streams, thirty categories),
# so a busy day or a smaller answer is not a failure and an empty one is.
LIVE = [
    ("a YouTube search", "https://www.youtube.com/results?search_query=lofi+music", 5, 0, 0),
    ("a YouTube video, with the ones beside it", "https://www.youtube.com/watch?v=dQw4w9WgXcQ", 3, 1, 5),
    # One of YouTube's own weekly charts, which it keeps.
    ("a YouTube playlist", "https://www.youtube.com/playlist?list=PL4fGSI1pDJn6jXS_Tv_N9B8Z0HTRVJE0m", 5, 0, 0),
    ("who is live on Twitch", "https://www.twitch.tv/", 5, 0, 0),
    ("Twitch's categories", "https://www.twitch.tv/directory", 5, 0, 0),
]
LIVE_LINE = r"SITETEST_LIVE page (-?\d+) rows (\d+) pictures (\d+) frames (\d+) comments (\d+)"


def resolved(vm, host, tries=4):
    """A name that will not resolve is not a fact about the reader. See
    tlscheck.resolved, which this is."""
    for _ in range(tries):
        if " is " in vm.fresh("resolve %s" % host, timeout=40):
            return True
    return False


def live(vm, url, tries=2):
    """What sitetest made of one real page: (page bytes, rows, pictures,
    storyboard sheets, everything it printed). Tried twice when nothing came
    back at all, and only then: a page that came back and read as nothing is
    the failure this is here to catch."""
    out = ""
    for _ in range(tries):
        out = vm.fresh("exec /bin/sitetest live %s" % url, timeout=240)
        m = re.search(LIVE_LINE, out)
        if m and int(m.group(1)) > 0:
            break
    m = re.search(LIVE_LINE, out)
    if m:
        return (int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)),
                int(m.group(5)), out)
    return None, 0, 0, 0, 0, out


def said(vm, before, timeout=240):
    """The browser's next line on the console after `before` of them, or
    None. It writes one each time a page has finished arriving."""
    end = time.time() + timeout
    while time.time() < end:
        lines = [l for l in vm.serial().splitlines() if l.startswith("browser: ")]
        if len(lines) > before:
            return lines[before]
        time.sleep(0.5)
    return None


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("sites read another way")

    vm = Guest(DISK, memory=256, extra=["-nic", "user,model=e1000"])
    try:
        vm.wait_boot()
        vm.run("dhcp", timeout=30)

        for host in ("www.youtube.com", "gql.twitch.tv"):
            c.add("%s resolves, so there is something to ask" % host, resolved(vm, host))

        # --- the reader, on its own ------------------------------------------
        for label, url, floor, sheets, talk in LIVE:
            size, rows, pics, frames, comments, out = live(vm, url)
            c.add("%s is read into a page" % label, size is not None and size > 0)
            c.add("with at least %d in it" % floor, rows >= floor)
            c.add("and a picture for each", size is not None and pics >= rows > 0)
            if sheets:
                c.add("and frames from the video, from its storyboards", frames >= sheets)
            if talk:
                c.add("and at least %d of its comments" % talk, comments >= talk)
            if size is None or size <= 0 or rows < floor:
                for line in out.splitlines()[1:8]:
                    print("      | %s" % line)

        # --- and in the browser ----------------------------------------------
        vm.type("desktop\n")
        time.sleep(6)
        mon = vm.monitor()

        # Started with nothing to go to, it opens on a page of its own.
        vm.type("browser\n")
        line = said(vm, 0) or ""
        print("      | %s" % line[:160])
        m = re.search(r"(\d+) links", line)
        c.add("the browser opens on its start page, with the sites on it",
              line.startswith("browser: about:start -- start --") and m is not None
              and int(m.group(1)) >= 6)

        steps = [
            ("https://www.youtube.com/results?search_query=lofi+music",
             lambda l: "read from the data in YouTube's page" in l and "lofi music - YouTube" in l
             and int((re.search(r"(\d+) pictures", l) or [0, 0])[1]) >= 5,
             "the browser shows a YouTube search as the reader's page, with its pictures"),
            ("https://www.youtube.com/watch?v=dQw4w9WgXcQ",
             lambda l: "read from the data in YouTube's page" in l
             and int((re.search(r"(\d+) pictures", l) or [0, 0])[1]) >= 10,
             "and a video's page, its picture, its frames and the ones beside it"),
            ("https://www.twitch.tv/directory",
             lambda l: "read from Twitch's API" in l and "Categories - Twitch" in l,
             "and Twitch's categories, from its API"),
            ("https://www.google.com/search?q=zelr+operating+system",
             lambda l: "lite.duckduckgo.com" in l and "q=zelr" in l
             and "answer to a Google search" in l
             and int((re.search(r"(\d+) links", l) or [0, 0])[1]) >= 5,
             "and a Google search is answered by DuckDuckGo's page"),
        ]
        for n, (url, good, label) in enumerate(steps, 1):
            mon.click(*BAR)
            time.sleep(1.0)
            vm.type(url + "\n")
            line = said(vm, n) or ""
            print("      | %s" % line[:160])
            c.add(label, good(line))
            mon.move_to(*PARK)

        # And the page says so, at its top, in the pale yellow of the note:
        # the note came before DuckDuckGo's <html> and went into the head,
        # where nothing is drawn, so the status said it and the page did not.
        time.sleep(1.5)
        w, h, px, shot = mon.screen("site-google")
        yellow = count_in(px, w, PAGE, (0xFF, 0xF4, 0xD6))
        c.add("and the page says at its top that it is DuckDuckGo's", yellow > 2000, shot)
        print("      the note's colour covers %d pixels" % yellow)
    finally:
        vm.stop()
        if not keep and os.path.exists(DISK):
            os.remove(DISK)

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
