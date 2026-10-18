"""Pictures, the picture viewer: a file shown, its folder walked, and Files
opening a picture in it rather than in the text editor.

Three PNGs are made here, in colours nothing else on the desktop uses, and
put in /home/pictures on a fresh volume. The viewer says on the console
what it shows ("pictures: NAME, W by H, N%"), which is what each step waits
for; the screen is then counted for the picture's own colours, so a line
said with nothing drawn would not pass.

  python tools/viewcheck.py [--keep]
"""
import os
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT          # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "viewcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "viewcheck.%d" % os.getpid())

TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
RED = (0xE0, 0x30, 0x40)
BLUE = (0x30, 0x50, 0xE0)
GREEN = (0x20, 0xC0, 0x60)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}


def png(path, w, h, pixel):
    rows = b"".join(b"\0" + b"".join(bytes(pixel(x, y)) for x in range(w)) for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("pictures")
    os.makedirs(WORK, exist_ok=True)
    a, b, big = (os.path.join(WORK, n) for n in ("a.png", "b.png", "c.png"))
    # a: red, with a blue square in it; b: green and small; c: big and red,
    # which has to be shrunk to fit.
    png(a, 300, 200, lambda x, y: BLUE if 100 <= x < 160 and 50 <= y < 110 else RED)
    png(b, 64, 48, lambda x, y: GREEN)
    png(big, 1600, 1200, lambda x, y: RED)
    # Written last first, so the folder lists c, b, a and only a viewer that
    # sorts by name walks a, b, c.
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768",
                        big + ":home/pictures/c.png", b + ":home/pictures/b.png",
                        a + ":home/pictures/a.png"], cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with three pictures on it", r.returncode == 0)
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
            "view-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def keys(text):
            for ch in text:
                mon.send("sendkey %s" % NAMED.get(ch, ch), settle=0.06)

        def said_since(mark, text, timeout=30):
            end = time.time() + timeout
            while time.time() < end:
                if text.lower() in vm.serial()[mark:].lower():   # mkfat's names are 8.3
                    return True
                time.sleep(0.2)
            return False

        mark = len(vm.serial())
        keys("/bin/pictures /home/pictures/a.png\n")
        shown = said_since(mark, "pictures: a.png, 300 by 200, 100%", 40)
        _, _, _, shot, drawn = mon.wait_screen(
            "view-a", lambda w, h, px: abs(count_in(px, w, SCREEN, RED) - (300 * 200 - 60 * 60)) < 600
            and abs(count_in(px, w, SCREEN, BLUE) - 60 * 60) < 200, timeout=20)
        c.add("a picture opened by name is shown whole, at its own size", shown and drawn, shot)

        mark = len(vm.serial())
        mon.send("sendkey right", settle=0.3)
        shown = said_since(mark, "pictures: b.png, 64 by 48, 100%")
        _, _, _, shot, drawn = mon.wait_screen(
            "view-b", lambda w, h, px: abs(count_in(px, w, SCREEN, GREEN) - 64 * 48) < 100
            and count_in(px, w, SCREEN, RED) < 100, timeout=20)
        c.add("the right arrow shows the next picture in the folder, and only it", shown and drawn, shot)

        mark = len(vm.serial())
        mon.send("sendkey right", settle=0.3)
        big_said = said_since(mark, "pictures: c.png, 1600 by 1200, ")
        line = [l for l in vm.serial()[mark:].splitlines() if l.lower().startswith("pictures: c.png")]
        pct = int(line[-1].rsplit(", ", 1)[1].rstrip("%\r")) if line else 0
        _, _, _, shot, fits = mon.wait_screen(
            "view-c", lambda w, h, px: 100000 < count_in(px, w, SCREEN, RED) < 720 * 540, timeout=20)
        c.add("a picture bigger than the window is shrunk to fit it (%d%%)" % pct,
              big_said and 0 < pct < 50 and fits, shot)

        mark = len(vm.serial())
        mon.send("sendkey left", settle=0.3)
        mon.send("sendkey left", settle=0.3)
        c.add("and the left arrow walks back", said_since(mark, "pictures: a.png, 300 by 200, 100%"))
        mon.send("sendkey alt-q", settle=1.0)

        # Files, which sent every file it did not know to the text editor.
        mark = len(vm.serial())
        keys("/bin/files /home/pictures\n")
        listed = said_since(mark, "files: /home/pictures", 30)
        chosen = False
        for _ in range(4):
            if not listed:
                break
            mon.send("sendkey down", settle=0.5)
            if said_since(mark, "files: a.png - picture", 3):
                chosen = True
                break
        c.add("Files calls a PNG a picture", chosen)
        mark = len(vm.serial())
        mon.send("sendkey ret", settle=0.5)
        c.add("and opens it in the viewer", said_since(mark, "pictures: a.png, 300 by 200, 100%", 40))
    finally:
        vm.stop()

    for f in (a, b, big):
        try:
            os.remove(f)
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
