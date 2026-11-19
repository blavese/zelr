"""The browser's right button menu.

Two pages and a picture on a fresh volume, opened as files (no network
needed): page m, grey, with a link to page n drawn as a green block, the
picture (orange, a PNG made here) and a blue block whose script keeps the
right button for a menu of its own. The browser says on the console what
rows a menu has as it opens ("browser menu: open the link; ...") and what a
row did ("browser menu: copied ...", "browser menu: saved ..."); each row is
clicked where the menu draws it, from the point that was right clicked.

  - on the link the menu offers the link's rows and the page's, and no
    "back" on the first page or "save the page" for a file;
  - "open it in a new window" opens n in a window of its own;
  - "copy the link's address", then ctrl+L, ctrl+V and return, goes to n;
  - "back" on n goes back to m;
  - on the picture: "save the picture" keeps the same bytes in
    /home/downloads, read off the disk image, and "open the picture" opens
    it;
  - a left press outside an open menu puts it away and the link under that
    press is not followed; the next click on it is;
  - the page's own block keeps the right button ("kept by the page") and
    the page heard it (its colour);
  - Escape puts a menu away, and a right press with the pointer at rest
    (no movement in its frame) opens one again.

  python tools/menucheck.py [--keep]
"""
import os
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT          # noqa: E402
from readfat import Fat16                                              # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "menucheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "menucheck.%d" % os.getpid())

TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
GREY = (0xE0, 0xE0, 0xE0)               # page m
BLUE = (0x30, 0x50, 0xE0)               # page n
TO_N = (0x30, 0xC0, 0x70)               # m's link to n
PIC = (0xE0, 0x80, 0x20)                # the picture
OWN = (0x40, 0x80, 0xC0)                # the block whose script keeps the right button
KEPT = (0x70, 0x70, 0x70)               # page m once its script heard the right button
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}

# browser.c's menu: rows of UI_ROW from UI_GAP below the point, at least
# MENU_W wide (as wide as its longest row).
UI_ROW, UI_GAP, MENU_W = 26, 6, 160

PAGE_M = b"""<!doctype html><html><head><title>page m</title><style>
body { margin: 0; background: #e0e0e0; min-height: 600px }
a { display: block; height: 80px; margin: 16px; color: white }
#ton { background: #30c070 }
img { display: block; margin: 16px; width: 160px; height: 80px }
#own { height: 60px; margin: 16px; background: #4080c0; color: white }
</style></head><body>
<a id="ton" href="n.htm">to n</a>
<img id="pic" src="p.png">
<div id="own">keeps the right button</div>
<script>
document.getElementById("own").addEventListener("contextmenu", function (e) {
  e.preventDefault(); document.body.style.background = "#707070";
});
</script></body></html>
"""
PAGE_N = b"""<!doctype html><html><head><title>page n</title><style>
body { margin: 0; background: #3050e0; min-height: 600px }</style></head><body><p>n</p></body></html>
"""


def png(w, h, rgb):
    """A picture of one colour, made with nothing but zlib."""
    row = b"\0" + bytes(rgb) * w
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(row * h, 9)) + chunk(b"IEND", b""))


PICTURE = png(160, 80, PIC)


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


def read(path):
    try:
        return Fat16(IMAGE).read_file(path)
    except Exception:          # gone, or a directory caught half written
        return None


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("the browser's right button menu")
    os.makedirs(WORK, exist_ok=True)
    files = {"m.htm": PAGE_M, "n.htm": PAGE_N, "p.png": PICTURE}
    for name, body in files.items():
        with open(os.path.join(WORK, name), "wb") as f:
            f.write(body)
    args = [os.path.join(WORK, n) + ":home/menu/" + n for n in files]
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with two pages and a picture on it", r.returncode == 0)
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
            "menu-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
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
            w, h, px, _ = mon.screen("menu-find")
            b = box_of(w, h, px, rgb)
            return None if not b else ((b[0] + b[2]) // 2, (b[1] + b[3]) // 2)

        def page_whole(name, rgb):
            return mon.wait_screen(name, lambda w, h, px: count_in(px, w, SCREEN, rgb) > 100000, timeout=40)

        def menu_at(at, name):
            """Right clicks at a point and gives back the menu's rows, as the
            browser said them, or None."""
            if not at:
                return None
            mark = len(vm.serial())
            mon.click(at[0], at[1], button=2)
            if not said_since(mark, "browser menu: ", 20):
                return None
            time.sleep(0.3)
            line = vm.serial()[mark:]
            line = line[line.find("browser menu: ") + 14:].split("\n")[0].strip()
            return line.split("; ")

        def choose(at, rows, row):
            """Clicks a row of the menu opened at `at`."""
            i = rows.index(row)
            mon.click(at[0] + 60, at[1] + UI_GAP + i * UI_ROW + UI_ROW // 2)

        mark = len(vm.serial())
        keys("/bin/browser /home/menu/m.htm\n")
        loaded = said_since(mark, "browser: file:///home/menu/m.htm -- page m", 60)
        _, _, _, shot, drawn = page_whole("menu-m", GREY)
        c.add("page m opens in the browser", loaded and drawn, shot)

        # On the link: its rows and the page's.
        link = centre(TO_N)
        rows = menu_at(link, "menu-link") or []
        want = ["open the link", "open it in a new window", "copy the link's address", "save what it links to",
                "reload", "copy the page's address"]
        c.add("the right button on a link offers its rows and the page's (no back yet, no saving a file)",
              rows == want, "; ".join(rows))

        mark = len(vm.serial())
        if "open it in a new window" in rows:
            choose(link, rows, "open it in a new window")
        told = said_since(mark, "browser window: a new one for file:///home/menu/n.htm", 30)
        came = said_since(mark, "browser: file:///home/menu/n.htm -- page n", 60)
        c.add("\"open it in a new window\" opens the link in a window of its own", told and came)
        if told:
            mon.send("sendkey alt-q", settle=1.0)
        _, _, _, shot, back = page_whole("menu-m2", GREY)
        c.add("and page m is still there after", back and times_said(mark, "browser: file:///home/menu/m.htm --") == 0,
              shot)

        # Copy the link's address, then paste it into the bar.
        link = centre(TO_N)
        rows = menu_at(link, "menu-link2") or []
        mark = len(vm.serial())
        if "copy the link's address" in rows:
            choose(link, rows, "copy the link's address")
        copied = said_since(mark, "browser menu: copied file:///home/menu/n.htm", 20)
        time.sleep(0.5)
        mark = len(vm.serial())
        mon.send("sendkey ctrl-l", settle=0.4)
        mon.send("sendkey ctrl-v", settle=0.4)
        mon.send("sendkey ret", settle=0.4)
        went = said_since(mark, "browser: file:///home/menu/n.htm -- page n", 60)
        c.add("\"copy the link's address\" copies it, and pasted into the bar it goes there", copied and went)
        _, _, _, shot, blue = page_whole("menu-n", BLUE)

        # Back, from the menu on page n.
        rows = menu_at((500, 400), "menu-n") or []
        mark = len(vm.serial())
        if "back" in rows:
            choose((500, 400), rows, "back")
        c.add("on the next page the menu has \"back\", and it goes back",
              blue and "back" in rows and said_since(mark, "browser: file:///home/menu/m.htm -- page m", 60),
              "; ".join(rows))
        page_whole("menu-m3", GREY)
        time.sleep(1.0)

        # The picture: save it, then open it.
        pic = centre(PIC)
        rows = menu_at(pic, "menu-pic") or []
        c.add("on the picture the menu offers its rows",
              all(r in rows for r in ("open the picture", "copy the picture's address", "save the picture")),
              "; ".join(rows))
        mark = len(vm.serial())
        if "save the picture" in rows:
            choose(pic, rows, "save the picture")
        saved = said_since(mark, "browser menu: saved /home/downloads/p.png, %d bytes, read back the same"
                           % len(PICTURE), 40)
        on_disk = False
        end = time.time() + 20
        while saved and time.time() < end and not on_disk:
            on_disk = read("/home/downloads/p.png") == PICTURE
            time.sleep(0.5)
        c.add("\"save the picture\" keeps the same bytes in /home/downloads", saved and on_disk)

        pic = centre(PIC)
        rows = menu_at(pic, "menu-pic2") or []
        mark = len(vm.serial())
        if "open the picture" in rows:
            choose(pic, rows, "open the picture")
        c.add("\"open the picture\" opens it", said_since(mark, "browser: file:///home/menu/p.png", 60))
        time.sleep(1.0)
        mark = len(vm.serial())
        mon.send("sendkey left", settle=0.4)            # back, from the keyboard
        said_since(mark, "browser: file:///home/menu/m.htm -- page m", 60)
        page_whole("menu-m4", GREY)
        time.sleep(1.0)

        # A press outside an open menu puts it away, and the link under the
        # press is not followed; the next click on it is.
        pic = centre(PIC)
        link = centre(TO_N)
        rows = menu_at(pic, "menu-pic3") or []
        mark = len(vm.serial())
        if rows and link:
            mon.click(link[0], link[1])
        time.sleep(3.0)
        followed = times_said(mark, "browser: file:///home/menu/n.htm --")
        if link:
            mon.click(link[0], link[1])
        went = said_since(mark, "browser: file:///home/menu/n.htm -- page n", 60)
        c.add("a press outside an open menu puts it away and follows nothing (%d); the next click does" % followed,
              bool(rows) and followed == 0 and went)
        page_whole("menu-n2", BLUE)
        mark = len(vm.serial())
        mon.send("sendkey left", settle=0.4)
        said_since(mark, "browser: file:///home/menu/m.htm -- page m", 60)
        page_whole("menu-m5", GREY)
        time.sleep(1.0)

        # The page's own block keeps the right button.
        own = centre(OWN)
        mark = len(vm.serial())
        if own:
            mon.click(own[0], own[1], button=2)
        kept = said_since(mark, "browser menu: kept by the page", 20)
        _, _, _, shot, heard = mon.wait_screen(
            "menu-kept", lambda w, h, px: count_in(px, w, SCREEN, KEPT) > 50000, timeout=20)
        c.add("a page that keeps the right button gets it, and the browser opens no menu",
              kept and heard and times_said(mark, "browser menu: open") == 0, shot)

        # Escape puts a menu away.
        spot = (own[0], own[1] - 140) if own else (500, 300)
        before = mon.screen("menu-before")
        rows = menu_at(spot, "menu-esc") or []
        # Clear of the pointer, which is left at the menu's corner.
        area = (spot[0] + 24, spot[1] + 4, spot[0] + MENU_W - 4, spot[1] + UI_GAP + len(rows) * UI_ROW - 4)
        w, h, px, shot_open = mon.screen("menu-open")
        b_w, _, b_px, _ = before
        drawn = count_in(px, w, area, KEPT) < count_in(b_px, b_w, area, KEPT) // 2
        mon.send("sendkey esc", settle=0.4)
        _, _, _, shot, gone = mon.wait_screen(
            "menu-esc", lambda w, h, px: count_in(px, w, area, KEPT) >= count_in(b_px, b_w, area, KEPT) - 10,
            timeout=15)
        c.add("a menu is drawn where it was asked for, and Escape puts it away", bool(rows) and drawn and gone, shot)

        # The right button again with the pointer where it was: a press with
        # no movement in its frame is a frame of its own, which the browser
        # once did not draw, so the menu did not open.
        mark = len(vm.serial())
        mon.send("mouse_button 2", settle=0.4)
        mon.send("mouse_button 0", settle=0.5)
        again = said_since(mark, "browser menu: ", 20)
        mon.send("sendkey esc", settle=0.4)
        c.add("a right press with the pointer at rest opens it again", again)
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
