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
from browsercheck import BAR, PARK                               # noqa: E402

DISK = os.path.join(ROOT, "sitecheck.%d.img" % os.getpid())

# What each page has to come to. The counts are floors well under what the
# sites give (twenty or so videos, twenty four streams, thirty categories),
# so a busy day or a smaller answer is not a failure and an empty one is.
LIVE = [
    ("a YouTube search", "https://www.youtube.com/results?search_query=lofi+music", 5),
    ("a YouTube video, with the ones beside it", "https://www.youtube.com/watch?v=dQw4w9WgXcQ", 3),
    ("who is live on Twitch", "https://www.twitch.tv/", 5),
    ("Twitch's categories", "https://www.twitch.tv/directory", 5),
]


def resolved(vm, host, tries=4):
    """A name that will not resolve is not a fact about the reader. See
    tlscheck.resolved, which this is."""
    for _ in range(tries):
        if " is " in vm.fresh("resolve %s" % host, timeout=40):
            return True
    return False


def live(vm, url, tries=2):
    """What sitetest made of one real page: (page bytes, rows, pictures,
    everything it printed). Tried twice when nothing came back at all, and
    only then: a page that came back and read as nothing is the failure this
    is here to catch."""
    out = ""
    for _ in range(tries):
        out = vm.fresh("exec /bin/sitetest live %s" % url, timeout=240)
        m = re.search(r"SITETEST_LIVE page (-?\d+) rows (\d+) pictures (\d+)", out)
        if m and int(m.group(1)) > 0:
            return int(m.group(1)), int(m.group(2)), int(m.group(3)), out
    m = re.search(r"SITETEST_LIVE page (-?\d+) rows (\d+) pictures (\d+)", out)
    if m:
        return int(m.group(1)), int(m.group(2)), int(m.group(3)), out
    return None, 0, 0, out


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
        for label, url, floor in LIVE:
            size, rows, pics, out = live(vm, url)
            c.add("%s is read into a page" % label, size is not None and size > 0)
            c.add("with at least %d in it" % floor, rows >= floor)
            c.add("and a picture for each", size is not None and pics >= rows > 0)
            if size is None or size <= 0 or rows < floor:
                for line in out.splitlines()[1:8]:
                    print("      | %s" % line)

        # --- and in the browser ----------------------------------------------
        vm.type("desktop\n")
        time.sleep(6)
        mon = vm.monitor()

        vm.type("browser https://www.youtube.com/results?search_query=lofi+music\n")
        line = said(vm, 0) or ""
        print("      | %s" % line[:160])
        c.add("the browser shows a YouTube search as the reader's page",
              "read from the data in YouTube's page" in line and "lofi music - YouTube" in line)
        m = re.search(r"(\d+) pictures", line)
        c.add("with its pictures", m is not None and int(m.group(1)) >= 5)

        steps = [
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
    finally:
        vm.stop()
        if not keep and os.path.exists(DISK):
            os.remove(DISK)

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
