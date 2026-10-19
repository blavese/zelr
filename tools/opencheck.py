"""Links opened in a window of their own.

Three pages on a fresh volume, opened as files (no network needed): page a,
red, with two links drawn as blocks of their own colours, one to page b
(blue) and one to page c (yellow) that a script keeps for itself as a
page's router does, unless ctrl was held. The browser says on the console
what it loaded and when it starts another window ("browser: a new window
for ADDRESS"); each step waits for that, then counts the screen.

  - the middle button on the link to b opens b in a new window; a stays on
    a, which shows beside b and fills the screen again once b is closed;
  - ctrl and a click on the same link does the same;
  - ctrl and a click on the router's link gets past the router, since the
    page is told ctrl was held, and opens c;
  - ctrl+N opens the start page in a new window.

  python tools/opencheck.py [--keep]
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT          # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "opencheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "opencheck.%d" % os.getpid())

TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
RED = (0xE0, 0x30, 0x40)                # page a
BLUE = (0x30, 0x50, 0xE0)               # page b
YELLOW = (0xE0, 0xC0, 0x20)             # page c
TO_B = (0x30, 0xC0, 0x70)               # page a's link to b
TO_C = (0xC0, 0x70, 0xC0)               # and its router's link to c
KEPT = (0x70, 0x70, 0x70)               # page a once its router kept a click
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}

PAGE_A = b"""<!doctype html><html><head><title>page a</title><style>
body { margin: 0; background: #e03040; min-height: 600px }
a { display: block; height: 150px; margin: 20px; color: white }
#tob { background: #30c070 }
#toc { background: #c070c0 }
</style></head><body>
<a id="tob" href="b.htm">to b</a>
<a id="toc" href="c.htm">to c</a>
<script>
document.getElementById("toc").addEventListener("click", function (e) {
  if (!e.ctrlKey) { e.preventDefault(); document.body.style.background = "#707070"; }
});
</script></body></html>
"""
PAGE_B = b"""<!doctype html><html><head><title>page b</title><style>
body { margin: 0; background: #3050e0; min-height: 600px }</style></head><body><p>b</p></body></html>
"""
PAGE_C = b"""<!doctype html><html><head><title>page c</title><style>
body { margin: 0; background: #e0c020; min-height: 600px }</style></head><body><p>c</p></body></html>
"""


def box_of(w, h, px, rgb):
    """The box around every pixel of a colour, or None."""
    x0 = y0 = 1 << 30
    x1 = y1 = -1
    for y in range(0, h, 2):
        row = y * w * 3
        for x in range(0, w, 2):
            i = row + x * 3
            if px[i] == rgb[0] and px[i + 1] == rgb[1] and px[i + 2] == rgb[2]:
                x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
    return None if x1 < 0 else (x0, y0, x1, y1)


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("links opened in a window of their own")
    os.makedirs(WORK, exist_ok=True)
    files = {"a.htm": PAGE_A, "b.htm": PAGE_B, "c.htm": PAGE_C}
    for name, body in files.items():
        with open(os.path.join(WORK, name), "wb") as f:
            f.write(body)
    args = [os.path.join(WORK, n) + ":home/open/" + n for n in files]
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with three pages on it", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    vm = Guest(IMAGE, memory=256, reuse=True, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "open-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def keys(text):
            for ch in text:
                mon.send("sendkey %s" % NAMED.get(ch, ch), settle=0.06)

        def said_since(mark, text, timeout=40):
            end = time.time() + timeout
            while time.time() < end:
                if text.lower() in vm.serial()[mark:].lower():   # mkfat's names are 8.3
                    return True
                time.sleep(0.2)
            return False

        def times_said(mark, text):
            return vm.serial()[mark:].lower().count(text.lower())

        def centre(rgb):
            w, h, px, _ = mon.screen("open-find")
            b = box_of(w, h, px, rgb)
            return None if not b else ((b[0] + b[2]) // 2, (b[1] + b[3]) // 2)

        def page_a_whole(name):
            return mon.wait_screen(name, lambda w, h, px: count_in(px, w, SCREEN, RED) > 100000, timeout=30)

        mark = len(vm.serial())
        keys("/bin/browser /home/open/a.htm\n")
        loaded = said_since(mark, "browser: file:///home/open/a.htm -- page a", 60)
        _, _, _, shot, drawn = page_a_whole("open-a")
        c.add("page a opens in the browser", loaded and drawn, shot)

        # The middle button on the link to b.
        mark = len(vm.serial())
        at = centre(TO_B)
        if at:
            mon.click(at[0], at[1], button=4)
        told = said_since(mark, "browser: a new window for file:///home/open/b.htm", 30)
        came = said_since(mark, "browser: file:///home/open/b.htm -- page b", 60)
        _, _, _, shot, both = mon.wait_screen(
            "open-mid", lambda w, h, px: count_in(px, w, SCREEN, BLUE) > 100000
            and count_in(px, w, SCREEN, RED) > 5000, timeout=30)
        c.add("the middle button on a link opens it in a new window", bool(at) and told and came, shot)
        c.add("and the first page shows beside it, still itself (b loaded %d times)"
              % times_said(mark, "browser: file:///home/open/b.htm --"),
              both and times_said(mark, "browser: file:///home/open/b.htm --") == 1, shot)
        if told:                            # only a window that came: else it is page a
            mon.send("sendkey alt-q", settle=1.0)
        _, _, _, shot, back = page_a_whole("open-mid-closed")
        c.add("closing the new window leaves page a, not loaded again",
              back and times_said(mark, "browser: file:///home/open/a.htm --") == 0, shot)

        # ctrl held and the left button, on the same link.
        mark = len(vm.serial())
        at = centre(TO_B)
        if at:
            mon.move_to(at[0], at[1])
            mon.send("sendkey ctrl 2500", settle=0.3)
            mon.send("mouse_button 1", settle=0.4)
            mon.send("mouse_button 0", settle=0.5)
        told = said_since(mark, "browser: a new window for file:///home/open/b.htm", 30)
        came = said_since(mark, "browser: file:///home/open/b.htm -- page b", 60)
        _, _, _, shot, both = mon.wait_screen(
            "open-ctrl", lambda w, h, px: count_in(px, w, SCREEN, BLUE) > 100000
            and count_in(px, w, SCREEN, RED) > 5000, timeout=30)
        c.add("so does a click with ctrl held", bool(at) and told and came and both, shot)
        if told:                            # only a window that came: else it is page a
            mon.send("sendkey alt-q", settle=1.0)
        time.sleep(2.0)                     # ctrl has come up
        _, _, _, shot, back = page_a_whole("open-ctrl-closed")
        c.add("and page a is still there after", back, shot)

        # The router's link: kept by the page for a plain click, it lets a
        # click with ctrl through because the page is told.
        mark = len(vm.serial())
        at = centre(TO_C)
        if at:
            mon.move_to(at[0], at[1])
            mon.send("sendkey ctrl 2500", settle=0.3)
            mon.send("mouse_button 1", settle=0.4)
            mon.send("mouse_button 0", settle=0.5)
        told = said_since(mark, "browser: a new window for file:///home/open/c.htm", 30)
        came = said_since(mark, "browser: file:///home/open/c.htm -- page c", 60)
        _, _, _, shot, seen = mon.wait_screen(
            "open-router", lambda w, h, px: count_in(px, w, SCREEN, YELLOW) > 100000, timeout=30)
        w, h, px, _ = mon.screen("open-router2")
        c.add("a page's router lets a click with ctrl through, told it was one, and c opens (kept %d)"
              % count_in(px, w, SCREEN, KEPT),
              bool(at) and told and came and seen and count_in(px, w, SCREEN, KEPT) < 100, shot)
        if told:                            # only a window that came: else it is page a
            mon.send("sendkey alt-q", settle=1.0)
        time.sleep(2.0)
        page_a_whole("open-router-closed")

        # ctrl+N.
        mark = len(vm.serial())
        mon.send("sendkey ctrl-n", settle=0.5)
        c.add("ctrl+N opens the start page in a new window",
              said_since(mark, "browser: a new window for about:start", 30)
              and said_since(mark, "browser: about:start --", 60))
    finally:
        vm.stop()

    for n in files:
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
