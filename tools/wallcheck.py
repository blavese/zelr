"""A picture as the wallpaper, set from Pictures with W, and taken away.

A picture red on its left half and blue on its right, 200 by 100, on a
fresh volume. Made the screen's size by covering it, the cut evenly at the
sides, its halves meet in the middle of the screen. The desktop and
Pictures say on the console what they did.

  - W in Pictures makes it the wallpaper: with everything put away, the
    left of the screen is red and the right blue;
  - the file it wrote goes, and the wallpaper chosen in Settings is back.

  python tools/wallcheck.py [--keep]
"""
import os
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from deskcheck import desk_icon_rect                         # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "wallcheck.%d.img" % os.getpid())
SRC = os.path.join(ROOT, "wallcheck.%d.png" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
RED = (0xE0, 0x30, 0x40)
BLUE = (0x30, 0x50, 0xE0)
LEFT = (120, 40, 500, 600)              # right of the icons, above the dock
RIGHT = (530, 40, 1000, 600)
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
    c = Checks("a picture as the wallpaper")
    png(SRC, 200, 100, lambda x, y: RED if x < 100 else BLUE)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768", SRC + ":home/pictures/w.png"],
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with a picture, red on the left and blue on the right", r.returncode == 0)
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
            "wall-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def keys(text):
            for ch in text:
                mon.send("sendkey %s" % NAMED.get(ch, ch), settle=0.06)

        def said_since(mark, text, timeout=30):
            end = time.time() + timeout
            while time.time() < end:
                if text.lower() in vm.serial()[mark:].lower():   # mkfat's names come back lower case
                    return True
                time.sleep(0.2)
            return False

        mark = len(vm.serial())
        keys("/bin/pictures /home/pictures/w.png\n")
        said_since(mark, "pictures: w.png, 200 by 100", 40)
        mark = len(vm.serial())
        mon.send("sendkey w", settle=0.5)
        c.add("W in Pictures sets it, and the desktop takes it",
              said_since(mark, "pictures: the wallpaper is w.png") and said_since(mark, "desktop: the wallpaper is a picture now"))

        mon.send("sendkey alt-d", settle=1.0)
        _, _, _, shot, shown = mon.wait_screen(
            "wall-shown", lambda w, h, px: count_in(px, w, LEFT, RED) > 150000 and count_in(px, w, RIGHT, BLUE) > 150000,
            timeout=30)
        c.add("with everything put away, the left of the screen is red and the right blue", shown, shot)

        # A terminal from the icon, to take the file away.
        x0, y0, x1, y1 = desk_icon_rect(0)
        mon.move_to((x0 + x1) // 2, y0 + 30)
        for _ in range(2):
            mon.send("mouse_button 1", settle=0.12)
            mon.send("mouse_button 0", settle=0.12)
        mon.move_to(1000, 20)
        mon.wait_screen("wall-term", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=30)
        time.sleep(1)
        mark = len(vm.serial())
        keys("rm /cfg/wallpaper.raw\n")
        gone = said_since(mark, "desktop: the wallpaper is no longer a picture")
        mon.send("sendkey alt-d", settle=1.0)
        _, _, _, shot, back = mon.wait_screen(
            "wall-gone", lambda w, h, px: count_in(px, w, LEFT, RED) < 1000 and count_in(px, w, RIGHT, BLUE) < 1000,
            timeout=30)
        c.add("taken away, the wallpaper chosen in Settings is back", gone and back, shot)
    finally:
        vm.stop()

    for junk in (SRC,) + (() if keep else (IMAGE,)):
        try:
            os.remove(junk)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
