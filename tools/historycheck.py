"""The browser's history: kept, listed, suggested, forgotten.

Three pages on a fresh volume, opened as files, the third kept as a bookmark
with ctrl+D on the way.
Every page that loads is a line of /home/history (when, address, title);
about:history lists them a day at a time; the bar suggests the bookmarks
and then the history, newest first, as an address is typed. The browser
says on the console what each page came to and what the bar suggests
("browser bar: suggests N: ADDRESS | ..."), and each step waits for that.

  - three pages visited are three lines of /home/history, read off the
    disk image here, stamped with the time (the machine on the host's
    clock);
  - about:history says it holds three, and still does after a restart;
  - the start page lists the three as visited lately;
  - typing a word of a title suggests that page, and down and return go;
  - typing a word every address has suggests the bookmark first, then the
    history newest first;
  - a suggestion clicked goes there;
  - a page on the disk with a link to about:history?forget cannot forget it;
  - after a page from the web server here, a page on the disk listed in
    about:history is opened by a click (the browser's own pages are no
    site's, so their links reach files; the links are drawn in the accent,
    one band of it an entry);
  - about:history's own "forget all of it" does.

  python tools/historycheck.py [--keep]
"""
import datetime
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT, HOST_CLOCK   # noqa: E402
from readfat import Fat16                                                  # noqa: E402
from webserver import Server                                               # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "historycheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "historycheck.%d" % os.getpid())

TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
ACCENT = (0x6E, 0x8A, 0xE8)             # the bar's ring and the go button
FORGET = (0xF3, 0xDE, 0xDE)             # about:history's forget button
TO_FORGET = (0x90, 0x40, 0xC0)          # page a's link to the same address
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret", ":": "shift-semicolon",
         "?": "shift-slash"}

PAGE = """<!doctype html><html><head><title>%s</title><style>
body { margin: 0; background: %s; min-height: 600px }
a { display: block; height: 120px; margin: 20px; background: #9040c0; color: white }
</style></head><body>%s</body></html>
"""
FILES = {
    "a.htm": PAGE % ("page alpha", "#e03040", '<a href="about:history?forget">forget</a>'),
    "b.htm": PAGE % ("page beta", "#3050e0", "<p>b</p>"),
    "c.htm": PAGE % ("page gamma", "#e0c020", "<p>c</p>"),
}


def box_of(px, w, area, rgb):
    x0 = y0 = 1 << 30
    x1 = y1 = -1
    for y in range(area[1], area[3]):
        row = y * w * 3
        for x in range(area[0], area[2]):
            i = row + x * 3
            if px[i] == rgb[0] and px[i + 1] == rgb[1] and px[i + 2] == rgb[2]:
                x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
    return None if x1 < 0 else (x0, y0, x1, y1)


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("the browser's history")
    os.makedirs(WORK, exist_ok=True)
    for name, body in FILES.items():
        with open(os.path.join(WORK, name), "w", newline="\n") as f:
            f.write(body)
    args = [os.path.join(WORK, n) + ":home/open/" + n for n in FILES]
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with three pages on it", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    def machine(extra=HOST_CLOCK):
        vm = Guest(IMAGE, memory=256, reuse=True, keep=True, extra=extra)
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "hist-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        time.sleep(1)
        return vm, mon, up, shot

    def keys(mon, text):
        for ch in text:
            mon.send("sendkey %s" % NAMED.get(ch, ch), settle=0.06)

    def said_since(vm, mark, text, timeout=40):
        end = time.time() + timeout
        while time.time() < end:
            if text.lower() in vm.serial()[mark:].lower():   # mkfat's names are 8.3
                return True
            time.sleep(0.2)
        return False

    def suggested(vm, mark):
        lines = [ln for ln in vm.serial()[mark:].splitlines() if "browser bar: suggests" in ln]
        return lines[-1].split("browser bar: ")[-1].strip() if lines else "nothing"

    def go(vm, mon, typed, wait_for, timeout=40):
        mark = len(vm.serial())
        mon.send("sendkey ctrl-l", settle=0.3)
        keys(mon, typed + "\n")
        return said_since(vm, mark, wait_for, timeout)

    vm, mon, up, shot = machine()
    try:
        c.add("the desktop comes up with its terminal", up, shot)
        before = datetime.datetime.utcnow()
        mark = len(vm.serial())
        keys(mon, "/bin/browser /home/open/a.htm\n")
        a = said_since(vm, mark, "browser: file:///home/open/a.htm -- page alpha", 60)
        g = go(vm, mon, "/home/open/c.htm", "browser: file:///home/open/c.htm -- page gamma")
        mark = len(vm.serial())
        mon.send("sendkey ctrl-d", settle=0.3)
        kept = said_since(vm, mark, "kept in the bookmarks: page gamma", 20)
        b = go(vm, mon, "/home/open/b.htm", "browser: file:///home/open/b.htm -- page beta")
        c.add("three pages are visited, the second kept as a bookmark", a and g and kept and b)
        c.add("about:history says it keeps three",
              go(vm, mon, "about:history", "browser: about:history -- history -- history, kept on this machine: 3 pages"))
        after = datetime.datetime.utcnow()
        mon.send("sendkey alt-q", settle=1.0)
        time.sleep(1.0)
    finally:
        vm.stop()

    # Off the disk, here.
    try:
        lines = Fat16(IMAGE).read_file("/home/history").decode("utf-8", "replace").splitlines()
    except Exception as e:
        print("  could not read /home/history: %s" % e)
        lines = []
    fields = [ln.split("\t") for ln in lines]
    c.add("/home/history holds a line a page, in the order visited (%d lines)" % len(lines),
          [f[1:] for f in fields] == [["file:///home/open/a.htm", "page alpha"],
                                      ["file:///home/open/c.htm", "page gamma"],
                                      ["file:///home/open/b.htm", "page beta"]])
    stamps = []
    for f in fields:
        try:
            stamps.append(datetime.datetime.strptime(f[0], "%Y-%m-%d %H:%M"))
        except (ValueError, IndexError):
            stamps.append(None)
    lo = before.replace(second=0, microsecond=0)
    c.add("each stamped with when, in the machine's time (%s)" % ", ".join(f[0] for f in fields if f),
          bool(stamps) and all(s is not None and lo <= s <= after for s in stamps))

    srv = Server()
    srv.__enter__()
    vm, mon, up, shot = machine(srv.qemu_args())
    try:
        mark = len(vm.serial())
        keys(mon, "/bin/browser about:history\n")
        c.add("the history is still there after a restart",
              said_since(vm, mark, "browser: about:history -- history -- history, kept on this machine: 3 pages", 60))
        c.add("and the start page lists the three as visited lately",
              go(vm, mon, "about:start", ", 3 visited lately"))

        # A word of a title, then down and return.
        mark = len(vm.serial())
        mon.send("sendkey ctrl-l", settle=0.3)
        keys(mon, "beta")
        c.add("a word of a title typed into the bar suggests that page",
              said_since(vm, mark, "browser bar: suggests 1: file:///home/open/b.htm", 20))
        mark = len(vm.serial())
        mon.send("sendkey down", settle=0.3)
        mon.send("sendkey ret", settle=0.3)
        c.add("and down and return go to it", said_since(vm, mark, "browser: file:///home/open/b.htm -- page beta"))

        # A word every address has: the bookmark, then the newest first
        # (b, just visited again; c is the bookmark, which goes first; a).
        mark = len(vm.serial())
        mon.send("sendkey ctrl-l", settle=0.3)
        keys(mon, "open")
        found = said_since(vm, mark, "browser bar: suggests 3: file:///home/open/c.htm | file:///home/open/b.htm"
                           " | file:///home/open/a.htm", 20)
        c.add("the bookmarks are suggested first, then the history, newest first (%s)" % suggested(vm, mark), found)
        mon.send("sendkey esc", settle=0.5)

        # Clicked: the list is under the bar, whose ring and go button are
        # the accent while it has the keyboard.
        mark = len(vm.serial())
        mon.send("sendkey ctrl-l", settle=0.3)
        keys(mon, "alpha")
        listed = said_since(vm, mark, "browser bar: suggests 1: file:///home/open/a.htm", 20)
        time.sleep(0.5)
        w, h, px, _ = mon.screen("hist-bar")
        bar = box_of(px, w, (0, 40, w, 220), ACCENT)
        mark = len(vm.serial())
        if listed and bar:
            mon.click(bar[0] + 80, bar[3] + 2 + 13)
        c.add("a suggestion clicked goes there (the bar at %s)" % (bar,),
              said_since(vm, mark, "browser: file:///home/open/a.htm -- page alpha"))

        # Page a's own link to about:history?forget: refused.
        w, h, px, _ = mon.screen("hist-a")
        link = box_of(px, w, (0, 40, w, 700), TO_FORGET)
        if link:
            mon.click((link[0] + link[2]) // 2, (link[1] + link[3]) // 2)
            time.sleep(2.0)
        c.add("a page's link to forget the history is not followed",
              bool(link) and go(vm, mon, "about:history",
                                "browser: about:history -- history -- history, kept on this machine: 3 pages"))

        # A page from the web, then about:history, whose second entry is page
        # a (the web page is the newest): a file, which a link on a web page
        # could not reach.
        web = go(vm, mon, "http://%s/peek" % srv.host, "a page that peeks", 60)
        listed = go(vm, mon, "about:history", "browser: about:history -- history -- history, kept on this machine: 4 pages")
        time.sleep(0.5)
        w, h, px, _ = mon.screen("hist-list")
        bands, inside = [], False
        # Inside the window only: the desktop's icons beside it are drawn in
        # the accent too. Its left edge is the bar's less the buttons.
        left = bar[0] - 150 if bar else 170
        for y in range(160, 700):
            row = y * w * 3
            hit = None
            for x in range(left, w):
                i = row + x * 3
                if px[i] == ACCENT[0] and px[i + 1] == ACCENT[1] and px[i + 2] == ACCENT[2]:
                    hit = x
                    break
            if hit is not None and not inside:
                # A link's letters and its underline are bands a few rows
                # apart; the next entry is twenty and more below.
                if bands and y - bands[-1][2] < 12:
                    bands[-1][0] = min(bands[-1][0], hit)
                else:
                    bands.append([hit, y, y])
            if hit is not None:
                bands[-1][2] = y
            inside = hit is not None
        mark = len(vm.serial())
        if len(bands) >= 2:
            x, top, bottom = bands[1]
            mon.click(x + 6, (top + bottom) // 2)
        c.add("after a web page, a file listed in about:history opens from it (%d links seen)" % len(bands),
              web and listed and len(bands) >= 2 and said_since(vm, mark, "browser: file:///home/open/a.htm -- page alpha"))

        # The history page's own button.
        go(vm, mon, "about:history", "browser: about:history --")
        time.sleep(0.5)
        w, h, px, _ = mon.screen("hist-page")
        button = box_of(px, w, (0, 40, w, 700), FORGET)
        mark = len(vm.serial())
        if button:
            mon.click((button[0] + button[2]) // 2, (button[1] + button[3]) // 2)
        c.add("about:history's forget all of it does",
              bool(button) and said_since(vm, mark, "history, kept on this machine: 0 pages"))
    finally:
        vm.stop()
        srv.__exit__(None, None, None)

    for n in FILES:
        try:
            os.remove(os.path.join(WORK, n))
        except OSError:
            pass
    try:
        os.rmdir(WORK)
    except OSError:
        pass
    if not keep:
        try:
            os.remove(IMAGE)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
